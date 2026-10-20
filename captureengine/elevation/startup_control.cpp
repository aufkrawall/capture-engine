#include "startup_control.h"
#include "elevation_client.h"
#include "setup_worker.h"
#include "common/setup/installer_setup_policy.h"
#include "common/logging/logging.h"
#include <shellapi.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <filesystem>
#include <atomic>

namespace ce::startup {
namespace {
using ce::elevation::Handle;
using ce::elevation::Transfer;
struct Context {
    uint32_t magic = 0x31534343;
    Action action = Action::Launch;
    uint32_t flags = 0;
    uint32_t administrator = 0;
    std::array<wchar_t, 192> owner{};
};
// Whether the owning user's account is an administrator (-1: not determined yet). The application's
// bootstrap, the elevated helper and the installer role state it; any other host, such as a client of
// the runtime that never ran Bootstrap, reads it from its own token on first use.
std::atomic<int> ownerAdministrator{-1};
SetupWorker setupWorker;

bool OwnerIsAdministrator() {
    int known = ownerAdministrator.load(std::memory_order_acquire);
    if (known < 0) {
        bool administrator = false;
        ce::elevation::ProcessUserSid(GetCurrentProcess(), &administrator);
        known = administrator ? 1 : 0;
        ownerAdministrator.store(known, std::memory_order_release);
    }
    return known != 0;
}

// The current process is the owning user: remember its SID and group membership.
void AdoptProcessOwner() {
    bool administrator = false;
    SetOwnerSid(ce::elevation::ProcessUserSid(GetCurrentProcess(), &administrator));
    ownerAdministrator.store(administrator ? 1 : 0, std::memory_order_release);
}

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
        LocalFree(static_cast<HLOCAL>(arguments));
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
    ownerAdministrator.store(context.administrator != 0 ? 1 : 0, std::memory_order_release);
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
    context.administrator = OwnerIsAdministrator() ? 1 : 0;
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
                                                     : ConfigureAutostart(desired, OwnerIsAdministrator());
        },
        [&] { return WritePreferences(desired); },
        [&] {
            return action == Action::InstallService || action == Action::RemoveService
                       ? old.service ? InstallElevationService() : RemoveElevationService()
                       : ConfigureAutostart(old, OwnerIsAdministrator());
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
    ownerAdministrator.store(request.ownerAdministrator ? 1 : 0, std::memory_order_release);
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
        LocalFree(static_cast<HLOCAL>(arguments));
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
        AdoptProcessOwner();
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
    AdoptProcessOwner();
    return LaunchRole(install ? Action::InstallService : Action::RemoveService, preferences);
}

namespace {
// One plan: a preference-only commit, the elevated helper (when this process is not elevated yet) or
// the in-process transaction. The decision is ExecutePlan's; the OS work is Apply/LaunchRole.
DWORD RunPlan(const SetupPlan& plan) {
    return ExecutePlan(
        plan, ce::elevation::IsElevated(), ERROR_WRITE_FAULT,
        [](const Preferences& desired) { return WritePreferences(desired); },
        [](const SetupPlan& launched) { return LaunchRole(launched.action, launched.desired); },
        [](const SetupPlan& applied) { return Apply(applied.action, applied.desired); });
}

DWORD StartWork(const char* what, std::function<unsigned long()> work) {
    if (!setupWorker.Start(std::move(work))) {
        LogInfo("[Startup] Setup request ignored: %s (another setup action is still in flight)", what);
        return ERROR_BUSY;
    }
    LogInfo("[Startup] Setup requested: %s", what);
    return ERROR_SUCCESS;
}

// alreadyApplied: the stored record and the OS state already match, so nothing is prompted or rewritten.
DWORD StartPlan(const SetupPlan& plan, bool alreadyApplied) {
    return StartWork(plan.preferenceOnly ? "preference-only" : ActionName(plan.action),
                     [plan, alreadyApplied]() -> unsigned long {
                         return alreadyApplied ? ERROR_SUCCESS : RunPlan(plan);
                     });
}

unsigned long ApplyPlainAutostart(const AutostartTarget& target, bool enable) {
    const std::optional<Registration> change = PlainAutostartChange(QueryClientAutostart(target), enable);
    return change ? ConfigureClientAutostart(target, *change) : ERROR_SUCCESS;
}
}  // namespace

bool Busy() {
    return setupWorker.Busy();
}

void Toggle(Setting setting) {
    if (Busy())
        return;
    const Preferences stored = ReadPreferences();
    StartPlan(PlanToggle(setting, stored, DisplayPreferences(), OwnerIsAdministrator()), false);
}

DWORD RequestElevationService(bool install) {
    if (Busy())
        return ERROR_BUSY;
    const Preferences stored = ReadPreferences();
    // Installing refreshes the staged runtime and so always runs; removing what is gone needs nothing.
    const bool alreadyRemoved =
        !install && !stored.service && QueryElevationServiceState() == ElevationServiceState::Absent;
    return StartPlan(PlanElevationService(stored, install), alreadyRemoved);
}

DWORD RequestClientAutostart(const AutostartTarget& target, bool enable) {
    if (!IsValidAutostartTarget(target))
        return ERROR_INVALID_PARAMETER;
    if (enable) {
        const DWORD attributes = GetFileAttributesW(target.executable.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return ERROR_FILE_NOT_FOUND;
    }
    if (Busy())
        return ERROR_BUSY;
    if (!IsApplicationAutostart(target))
        return StartWork(enable ? "client-autostart-on" : "client-autostart-off",
                         [target, enable]() -> unsigned long { return ApplyPlainAutostart(target, enable); });
    const Preferences stored = ReadPreferences();
    const bool administrator = OwnerIsAdministrator();
    const SetupPlan plan = PlanApplicationAutostart(stored, enable, administrator);
    return StartPlan(plan, AutostartPlanApplied(plan, stored, QueryClientAutostart(target), administrator));
}

DWORD RequestLaunchElevated(bool elevated) {
    if (Busy())
        return ERROR_BUSY;
    const Preferences stored = ReadPreferences();
    const SetupPlan plan = PlanLaunchElevated(stored, elevated, OwnerIsAdministrator());
    return StartPlan(plan, stored.elevated == elevated);
}

std::optional<unsigned long> TakeSetupResult() {
    return setupWorker.TakeResult();
}

std::optional<unsigned long> WaitSetupResult() {
    return setupWorker.WaitForResult();
}

void Pump() {
    const std::optional<unsigned long> taken = setupWorker.TakeResult();
    if (!taken)
        return;
    const DWORD result = *taken;
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
    setupWorker.Shutdown();
}

ElevationServiceState QueryServiceState(const wchar_t* serviceName) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return ElevationServiceState::Unknown;
    SC_HANDLE service = OpenServiceW(manager, serviceName, SERVICE_QUERY_STATUS);
    const DWORD openError = service ? ERROR_SUCCESS : GetLastError();
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    const bool queried =
        service && QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                                        sizeof(status), &bytes);
    if (service)
        CloseServiceHandle(service);
    CloseServiceHandle(manager);
    // A service this account may not open is unknown, not absent.
    if (!service)
        return openError == ERROR_SERVICE_DOES_NOT_EXIST ? ElevationServiceState::Absent
                                                         : ElevationServiceState::Unknown;
    if (!queried)
        return ElevationServiceState::Unknown;
    return status.dwCurrentState == SERVICE_RUNNING ? ElevationServiceState::Running
                                                    : ElevationServiceState::Installed;
}

ElevationServiceState QueryElevationServiceState() {
    return QueryServiceState(ce::elevation::kServiceName);
}

Preferences DisplayPreferences() {
    Preferences preferences = ReadPreferences();
    preferences.service = preferences.service && ServiceRegistered(QueryElevationServiceState());
    preferences.autostart = preferences.autostart && AutostartMatches(preferences, OwnerIsAdministrator());
    return preferences;
}

std::wstring ServiceStatusText() {
    return ElevationServiceLabel(QueryElevationServiceState());
}
}  // namespace ce::startup
