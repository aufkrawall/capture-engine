// Everything that crosses into the installed program: the elevated service and
// autostart role, the PawnIO driver role, and launching it again as the user.
//
// Capture Engine owns its service staging, its startup registration and its
// driver setup (captureengine/elevation_setup.cpp, startup_autostart.cpp,
// pawnio_setup.cpp). Setup deliberately calls the installed program's own roles
// instead of re-implementing them, so an installation and the tray toggles can
// never disagree about what "service enabled" means.

#include "setup.h"

#include <userenv.h>

#include "../common/elevation_windows.h"

namespace ce::setup {
namespace {

constexpr DWORD kRoleTimeoutMilliseconds = 3 * 60 * 1000;
constexpr DWORD kPawnIoTimeoutMilliseconds = 11 * 60 * 1000;

// Runs an elevated child without a window and waits for it. The child inherits
// setup's elevation, so no further prompt appears.
DWORD RunHidden(const std::wstring& exe, const std::wstring& arguments, const std::wstring& workDir,
                DWORD timeoutMilliseconds) {
    std::wstring commandLine = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        workDir.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        Log("role: cannot start %s (error %lu)", Narrow(exe).c_str(), error);
        return error;
    }
    Handle thread(process.hThread);
    Handle child(process.hProcess);
    if (WaitForSingleObject(child.Get(), timeoutMilliseconds) != WAIT_OBJECT_0) {
        Log("role: %s did not finish in time; terminating it", Narrow(arguments).c_str());
        TerminateProcess(child.Get(), ERROR_TIMEOUT);
        WaitForSingleObject(child.Get(), 10000);
        return ERROR_TIMEOUT;
    }
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(child.Get(), &exitCode);
    return exitCode;
}

}  // namespace

bool IsPawnIoInstalled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\PawnIO", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

DWORD InteractiveShellProcess() {
    DWORD processId = 0;
    if (HWND shell = GetShellWindow())
        GetWindowThreadProcessId(shell, &processId);
    return processId;
}

bool ResolveOwner(std::wstring* sid, bool* administrator) {
    // Who the installation is for: the person at the desktop. Setup itself may
    // run as a different administrator account that was entered at the prompt.
    bool admin = false;
    std::wstring result;
    if (const DWORD shell = InteractiveShellProcess()) {
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shell));
        if (process.Valid())
            result = ce::elevation::ProcessUserSid(process.Get(), &admin);
    }
    if (result.empty())
        result = ce::elevation::ProcessUserSid(GetCurrentProcess(), &admin);
    if (result.empty())
        return false;
    *sid = result;
    *administrator = admin;
    return true;
}

std::wstring RegisteredServiceOwner() {
    wchar_t owner[256] = {};
    DWORD bytes = sizeof(owner);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\CaptureEngineElevation\\Parameters",
                     L"OwnerSid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, owner, &bytes) != ERROR_SUCCESS)
        return {};
    return owner;
}

DWORD RunIntegrationRole(const IntegrationRequest& request) {
    const std::wstring exe = JoinPath(request.directory, kAppExe);
    if (!PathExists(exe))
        return ERROR_FILE_NOT_FOUND;
    uint32_t preferences = 0;
    if (request.options & kOptService)
        preferences |= 1;
    if (request.options & kOptElevated)
        preferences |= 2;
    if (request.options & kOptAutostart)
        preferences |= 4;
    const wchar_t* service = request.applyService ? L"install" : request.removeService ? L"remove" : L"keep";
    std::wstring arguments = L"--ce-installer-setup --owner-sid=" + request.ownerSid +
                             L" --owner-admin=" + (request.ownerAdministrator ? L"1" : L"0") +
                             L" --prefs=" + std::to_wstring(preferences) + L" --service=" + service +
                             L" --autostart=" + (request.applyAutostart ? L"apply" : L"keep");
    Log("role: installer-setup service=%ls autostart=%ls prefs=%u", service, request.applyAutostart ? L"apply" : L"keep",
        preferences);
    const DWORD result = RunHidden(exe, arguments, request.directory, kRoleTimeoutMilliseconds);
    Log("role: installer-setup finished with %lu", static_cast<unsigned long>(result));
    return result;
}

DWORD RunPawnIoRole(const std::wstring& directory) {
    const std::wstring exe = JoinPath(directory, kAppExe);
    if (!PathExists(exe))
        return ERROR_FILE_NOT_FOUND;
    Log("role: PawnIO driver setup");
    const DWORD result = RunHidden(exe, L"--install-pawnio", directory, kPawnIoTimeoutMilliseconds);
    Log("role: PawnIO driver setup finished with %lu", static_cast<unsigned long>(result));
    return result;
}

bool LaunchAsInteractiveUser(const std::wstring& exePath, const std::wstring& arguments, const std::wstring& workDir) {
    // Setup is elevated; the program must not inherit that. Borrow the desktop
    // shell's token - the same unelevated identity a double-click would use.
    const DWORD shell = InteractiveShellProcess();
    if (!shell) {
        Log("launch: no desktop shell is running");
        return false;
    }
    Handle shellProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shell));
    HANDLE rawToken = nullptr;
    if (!shellProcess.Valid() || !OpenProcessToken(shellProcess.Get(), TOKEN_DUPLICATE | TOKEN_QUERY, &rawToken)) {
        Log("launch: cannot open the shell token (error %lu)", GetLastError());
        return false;
    }
    Handle shellToken(rawToken);
    HANDLE rawPrimary = nullptr;
    if (!DuplicateTokenEx(shellToken.Get(), TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary,
                          &rawPrimary)) {
        Log("launch: cannot duplicate the shell token (error %lu)", GetLastError());
        return false;
    }
    Handle primary(rawPrimary);
    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary.Get(), FALSE))
        environment = nullptr;
    std::wstring commandLine = L"\"" + exePath + L"\"" + (arguments.empty() ? L"" : L" " + arguments);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessWithTokenW(primary.Get(), LOGON_WITH_PROFILE, exePath.c_str(),
                                                 commandLine.data(), CREATE_UNICODE_ENVIRONMENT, environment,
                                                 workDir.c_str(), &startup, &process);
    const DWORD error = GetLastError();
    if (environment)
        DestroyEnvironmentBlock(environment);
    if (!started) {
        Log("launch: CreateProcessWithTokenW failed (error %lu)", error);
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

}  // namespace ce::setup
