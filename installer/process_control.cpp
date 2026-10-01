// Closing a running Capture Engine and its elevation service before files are
// replaced or removed.
//
// Order matters: the controller is asked to quit through its own window so it
// can finalise a recording and stop its child processes itself; only what is
// still alive after a bounded grace period is terminated. Games are never
// touched - a hook DLL loaded into one is handled by renaming the file away
// (see engine_install.cpp), not by killing the game.

#include "setup.h"

#include <tlhelp32.h>

#include <algorithm>
#include <set>

namespace ce::setup {
namespace {

constexpr DWORD kTerminateWaitMilliseconds = 10000;
constexpr DWORD kServiceStopMilliseconds = 20000;

struct WindowSearch {
    const std::set<DWORD>* processes;
    size_t posted;
};

BOOL CALLBACK PostCloseToWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (search->processes->count(processId)) {
        // Posted, not sent: a hung window must not block setup.
        PostMessageW(window, WM_CLOSE, 0, 0);
        ++search->posted;
    }
    return TRUE;
}

std::vector<DWORD> FindProcessesUnder(const std::wstring& directory) {
    std::vector<DWORD> found;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid())
        return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    if (!Process32FirstW(snapshot.Get(), &entry))
        return found;
    do {
        if (entry.th32ProcessID == 0 || entry.th32ProcessID == 4 || entry.th32ProcessID == self)
            continue;
        const std::wstring image = ProcessImagePath(entry.th32ProcessID);
        if (!image.empty() && IsPathInside(image, directory))
            found.push_back(entry.th32ProcessID);
    } while (Process32NextW(snapshot.Get(), &entry));
    return found;
}

// Waits until every process in `processes` has exited or `timeout` elapsed.
bool WaitForExit(const std::vector<DWORD>& processes, DWORD timeout) {
    std::vector<Handle> handles;
    for (DWORD processId : processes) {
        Handle process(OpenProcess(SYNCHRONIZE, FALSE, processId));
        if (process.Valid())
            handles.push_back(std::move(process));
    }
    const ULONGLONG deadline = GetTickCount64() + timeout;
    for (size_t first = 0; first < handles.size(); first += MAXIMUM_WAIT_OBJECTS) {
        const DWORD count = static_cast<DWORD>(std::min<size_t>(MAXIMUM_WAIT_OBJECTS, handles.size() - first));
        std::vector<HANDLE> raw;
        raw.reserve(count);
        for (DWORD index = 0; index < count; ++index)
            raw.push_back(handles[first + index].Get());
        const ULONGLONG now = GetTickCount64();
        const DWORD remaining = now < deadline ? static_cast<DWORD>(deadline - now) : 0;
        if (WaitForMultipleObjects(count, raw.data(), TRUE, remaining) == WAIT_TIMEOUT)
            return false;
    }
    return true;
}

}  // namespace

bool ElevationServiceExists() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager)
        return false;
    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS);
    const bool exists = service != nullptr;
    if (service)
        CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return exists;
}

bool StopElevationService(std::wstring* error) {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        if (error)
            *error = L"The service manager could not be opened: " + ErrorText(GetLastError());
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, kServiceName, SERVICE_QUERY_STATUS | SERVICE_STOP);
    if (!service) {
        const DWORD code = GetLastError();
        CloseServiceHandle(manager);
        if (code == ERROR_SERVICE_DOES_NOT_EXIST)
            return true;
        if (error)
            *error = L"The Capture Engine service could not be opened: " + ErrorText(code);
        return false;
    }
    bool ok = true;
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status),
                             &bytes) &&
        status.dwCurrentState != SERVICE_STOPPED) {
        // The process handle is the wait object: STOPPED in the service manager
        // is a status, not proof that the image mappings have gone away.
        Handle process(status.dwProcessId ? OpenProcess(SYNCHRONIZE, FALSE, status.dwProcessId) : nullptr);
        SERVICE_STATUS stopped{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &stopped) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
            const DWORD code = GetLastError();
            Log("service: stop request failed (error %lu)", code);
            if (error)
                *error = L"The Capture Engine service could not be stopped: " + ErrorText(code);
            ok = false;
        } else if (process.Valid() && WaitForSingleObject(process.Get(), kServiceStopMilliseconds) != WAIT_OBJECT_0) {
            if (error)
                *error = L"The Capture Engine service did not stop in time.";
            ok = false;
        } else {
            Log("service: stopped");
        }
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return ok;
}

bool CloseRunningInstances(const std::wstring& directory, std::wstring* error, const ProgressFn& progress,
                           bool stopService, unsigned graceSeconds) {
    std::vector<DWORD> running = FindProcessesUnder(directory);
    if (!running.empty()) {
        Log("close: %zu process(es) run from the installation folder", running.size());
        if (progress)
            progress(-1, L"Closing Capture Engine...");
        const std::set<DWORD> targets(running.begin(), running.end());
        WindowSearch search{&targets, 0};
        EnumWindows(PostCloseToWindow, reinterpret_cast<LPARAM>(&search));
        Log("close: asked %zu window(s) to close", search.posted);
        if (!WaitForExit(running, graceSeconds * 1000)) {
            running = FindProcessesUnder(directory);
            Log("close: %zu process(es) still alive after the grace period; terminating", running.size());
            for (DWORD processId : running) {
                Handle process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId));
                if (process.Valid() && !TerminateProcess(process.Get(), 1))
                    Log("close: terminate of process %lu failed (error %lu)", static_cast<unsigned long>(processId),
                        GetLastError());
            }
            WaitForExit(running, kTerminateWaitMilliseconds);
        }
        running = FindProcessesUnder(directory);
        if (!running.empty()) {
            if (error)
                *error = L"Capture Engine is still running from this folder and could not be closed. "
                         L"Close it from its tray icon and run setup again.";
            Log("close: %zu process(es) could not be closed", running.size());
            return false;
        }
    }
    if (!stopService)
        return true;
    std::wstring serviceError;
    if (!StopElevationService(&serviceError)) {
        // A service that cannot be stopped does not hold the files being
        // replaced (its runtime is a separate protected copy); report, go on.
        Log("close: %s", Narrow(serviceError).c_str());
    }
    return true;
}

}  // namespace ce::setup
