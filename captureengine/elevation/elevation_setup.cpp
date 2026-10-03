#include "startup_control.h"
#include "elevation_client.h"
#include "common/setup/elevation_runtime_policy.h"
#include "common/logging/logging.h"
#include <shlobj.h>
#include <filesystem>
#include <array>
#include <bcrypt.h>

namespace ce::startup {
namespace {
using ce::elevation::Security;
namespace fs = std::filesystem;

struct ServiceHandles {
    SC_HANDLE manager = nullptr;
    SC_HANDLE service = nullptr;
    ~ServiceHandles() {
        if (service)
            CloseServiceHandle(service);
        if (manager)
            CloseServiceHandle(manager);
    }
};

fs::path ServiceDirectory() {
    return ServiceDirectoryForExecutable(ce::elevation::ExecutablePath());
}

fs::path LegacyServiceParent() {
    wchar_t* programFiles = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &programFiles)))
        return {};
    fs::path result = fs::path(programFiles) / L"CaptureEngine";
    CoTaskMemFree(programFiles);
    return result;
}

bool SafeDirectory(const fs::path& directory, bool create) {
    Security security(L"O:BAG:BAD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGX;;;BU)");
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            SetLastError(ERROR_ACCESS_DENIED);
            return false;
        }
        return !create ||
               (security.Descriptor() && SetFileSecurityW(directory.c_str(),
                                                          OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                                                              PROTECTED_DACL_SECURITY_INFORMATION,
                                                          security.Descriptor()));
    }
    if (!create)
        return true;
    return security.Get() && CreateDirectoryW(directory.c_str(), security.Get());
}

DWORD ValidateRegistration(SC_HANDLE service, fs::path* runtime) {
    DWORD bytes = 0;
    QueryServiceConfigW(service, nullptr, 0, &bytes);
    std::vector<BYTE> storage(bytes);
    if (!bytes ||
        !QueryServiceConfigW(service, reinterpret_cast<QUERY_SERVICE_CONFIGW*>(storage.data()), bytes, &bytes))
        return GetLastError();
    const auto* config = reinterpret_cast<const QUERY_SERVICE_CONFIGW*>(storage.data());
    const fs::path candidate = RegisteredServiceRuntime(config->lpBinaryPathName);
    if (config->dwServiceType != SERVICE_WIN32_OWN_PROCESS ||
        _wcsicmp(config->lpServiceStartName, L"LocalSystem") != 0 || candidate.empty() ||
        !SafeDirectory(candidate.parent_path().parent_path(), false) ||
        !SafeDirectory(candidate.parent_path(), false) || !SafeDirectory(candidate, false))
        return ERROR_INVALID_DATA;
    wchar_t owner[256]{};
    bytes = sizeof(owner);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\CaptureEngineElevation\\Parameters",
                     L"OwnerSid", RRF_RT_REG_SZ, nullptr, owner, &bytes) != ERROR_SUCCESS ||
        owner != OwnerSid())
        return ERROR_ACCESS_DENIED;
    *runtime = candidate;
    return ERROR_SUCCESS;
}

DWORD RemoveRuntime(const fs::path& runtime) {
    // Delete only the validated runtime; keep unrelated files and install roots.
    if (!SafeDirectory(runtime.parent_path().parent_path(), false) ||
        !SafeDirectory(runtime.parent_path(), false) || !SafeDirectory(runtime, false))
        return ERROR_ACCESS_DENIED;
    std::error_code error;
    fs::remove_all(runtime, error);
    if (error)
        return static_cast<DWORD>(error.value());
    RemoveDirectoryW(runtime.parent_path().c_str());
    const fs::path legacy = LegacyServiceParent();
    if (!legacy.empty() && runtime.parent_path().parent_path() == legacy &&
        legacy != ServiceDirectory().parent_path())
        RemoveDirectoryW(legacy.c_str());
    return ERROR_SUCCESS;
}

DWORD StopService(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status),
                              &bytes))
        return GetLastError();
    if (status.dwCurrentState == SERVICE_STOPPED)
        return ERROR_SUCCESS;
    ce::elevation::Handle process(OpenProcess(SYNCHRONIZE, FALSE, status.dwProcessId));
    SERVICE_STATUS stopped{};
    if (!ControlService(service, SERVICE_CONTROL_STOP, &stopped) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
        return GetLastError();
    if (!ce::elevation::WaitServiceState(service, SERVICE_STOPPED, 15000))
        return GetLastError();
    // STOPPED is a service status, not proof that DLL mappings have gone away.
    if (!process)
        return ERROR_ACCESS_DENIED;
    if (WaitForSingleObject(process.Get(), 15000) != WAIT_OBJECT_0)
        return ERROR_TIMEOUT;
    return ERROR_SUCCESS;
}

DWORD CopyRuntime(const fs::path& source, const fs::path& destination) {
    if (!SafeDirectory(destination.parent_path(), true) || !SafeDirectory(destination, true))
        return GetLastError();
    Security security(L"O:BAG:BAD:P(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGX;;;BU)");
    if (!security.Descriptor() ||
        !SetFileSecurityW(destination.c_str(),
                          OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                          security.Descriptor()))
        return GetLastError();
    const std::array<fs::path, 7> files = {
        L"captureengine_elevation_service.exe",
        L"plugins/LibreHardwareMonitor/LibreHardwareMonitorLib.dll",
        L"plugins/LibreHardwareMonitor/System.Memory.dll",
        L"plugins/LibreHardwareMonitor/System.Numerics.Vectors.dll",
        L"plugins/LibreHardwareMonitor/System.Runtime.CompilerServices.Unsafe.dll",
        L"licenses/MIT_CaptureEngine.txt",
        L"licenses/LibreHardwareMonitor_NOTICE.txt",
    };
    for (const fs::path& relative : files) {
        fs::path parent = destination;
        const auto components = relative.parent_path();
        for (const auto& component : components) {
            parent /= component;
            if (!SafeDirectory(parent, true))
                return GetLastError();
        }
        const DWORD attributes = GetFileAttributesW((source / relative).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
            return ERROR_FILE_NOT_FOUND;
        const DWORD outputAttributes = GetFileAttributesW((destination / relative).c_str());
        if (outputAttributes != INVALID_FILE_ATTRIBUTES && outputAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            return ERROR_ACCESS_DENIED;
        if (!CopyFileW((source / relative).c_str(), (destination / relative).c_str(), FALSE))
            return GetLastError();
        if (!SetFileSecurityW(
                (destination / relative).c_str(),
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                security.Descriptor()))
            return GetLastError();
    }
    const fs::path symbols = source / L"captureengine_elevation_service.pdb";
    const DWORD symbolAttributes = GetFileAttributesW(symbols.c_str());
    if (symbolAttributes != INVALID_FILE_ATTRIBUTES &&
        !(symbolAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        const fs::path output = destination / symbols.filename();
        if (!CopyFileW(symbols.c_str(), output.c_str(), FALSE) ||
            !SetFileSecurityW(
                output.c_str(),
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                security.Descriptor()))
            return GetLastError();
    }
    return ValidateSensorRuntime(destination.wstring());
}
}  // namespace

DWORD InstallElevationService() {
    if (!ce::elevation::IsElevated())
        return ERROR_ELEVATION_REQUIRED;
    const fs::path directory = ServiceDirectory();
    // The application folder keeps its own ACL; only the service subtree is protected.
    if (directory.empty() || !SafeDirectory(directory.parent_path(), false) || !SafeDirectory(directory, true))
        return ERROR_ACCESS_DENIED;
    ServiceHandles handles;
    handles.manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!handles.manager)
        return GetLastError();
    handles.service = OpenServiceW(handles.manager, ce::elevation::kServiceName, SERVICE_ALL_ACCESS);
    fs::path oldRuntime;
    bool wasRunning = false;
    if (handles.service) {
        const DWORD valid = ValidateRegistration(handles.service, &oldRuntime);
        if (valid != ERROR_SUCCESS)
            return valid;
        SERVICE_STATUS_PROCESS status{};
        DWORD bytes = 0;
        if (!QueryServiceStatusEx(handles.service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                                  sizeof(status), &bytes))
            return GetLastError();
        wasRunning = status.dwCurrentState != SERVICE_STOPPED;
        LogInfo("[ElevationService] Replacing registered runtime (relocating=%d)", oldRuntime.parent_path() != directory);
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        return GetLastError();
    std::array<unsigned char, 16> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return ERROR_GEN_FAILURE;
    std::wstring leaf = L"runtime-";
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (unsigned char value : nonce) {
        leaf.push_back(hex[value >> 4]);
        leaf.push_back(hex[value & 15]);
    }
    const fs::path runtime = directory / leaf;
    const DWORD copy = CopyRuntime(fs::path(ce::elevation::ExecutablePath()).parent_path(), runtime);
    if (copy != ERROR_SUCCESS) {
        std::error_code ignored;
        if (SafeDirectory(runtime, false))
            fs::remove_all(runtime, ignored);
        return copy;
    }
    const auto discardStaging = [&] {
        std::error_code error;
        fs::remove_all(runtime, error);
        if (error)
            LogWarn("[ElevationService] Staged runtime cleanup incomplete (error=%d)", error.value());
    };
    if (handles.service) {
        if (!ChangeServiceConfigW(handles.service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            const DWORD error = GetLastError();
            discardStaging();
            return error;
        }
        const DWORD stop = StopService(handles.service);
        if (stop != ERROR_SUCCESS) {
            ChangeServiceConfigW(handles.service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            discardStaging();
            return stop;
        }
    }
    const std::wstring binary =
        ce::elevation::QuoteArgument((runtime / L"captureengine_elevation_service.exe").wstring());
    bool created = false;
    if (!handles.service) {
        handles.service =
            CreateServiceW(handles.manager, ce::elevation::kServiceName, L"CaptureEngine Elevation Service",
                           SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                           binary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
        created = true;
    }
    if (!handles.service) {
        const DWORD error = GetLastError();
        discardStaging();
        return error;
    }
    Security security(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;CCLCRP;;;" + OwnerSid() + L")");
    DWORD result = ERROR_SUCCESS;
    if (!security.Descriptor() ||
        !SetServiceObjectSecurity(handles.service, DACL_SECURITY_INFORMATION, security.Descriptor()))
        result = GetLastError();
    HKEY parameters = nullptr;
    if (result == ERROR_SUCCESS) {
        result = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                                 L"SYSTEM\\CurrentControlSet\\Services\\CaptureEngineElevation\\Parameters", 0, nullptr,
                                 0, KEY_SET_VALUE, nullptr, &parameters, nullptr);
        if (result == ERROR_SUCCESS) {
            const std::wstring owner = OwnerSid();
            result = RegSetValueExW(parameters, L"OwnerSid", 0, REG_SZ, reinterpret_cast<const BYTE*>(owner.c_str()),
                                    static_cast<DWORD>((owner.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(parameters);
        }
    }
    if (result == ERROR_SUCCESS &&
        !ChangeServiceConfigW(handles.service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE,
                              binary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
        result = GetLastError();
    if (result == ERROR_SUCCESS && !StartServiceW(handles.service, 0, nullptr))
        result = GetLastError();
    if (result == ERROR_SUCCESS && !ce::elevation::WaitServiceState(handles.service, SERVICE_RUNNING, 10000))
        result = GetLastError();
    if (result != ERROR_SUCCESS && created) {
        const DWORD stopped = StopService(handles.service);
        if (stopped == ERROR_SUCCESS && DeleteService(handles.service)) {
            CloseServiceHandle(handles.service);
            handles.service = nullptr;
            discardStaging();
        } else {
            LogError("[ElevationService] Failed installation cleanup incomplete (error=%lu)", stopped);
            result = stopped ? stopped : GetLastError();
        }
    } else if (result != ERROR_SUCCESS && !oldRuntime.empty()) {
        const DWORD stopped = StopService(handles.service);
        const std::wstring oldBinary =
            ce::elevation::QuoteArgument((oldRuntime / L"captureengine_elevation_service.exe").wstring());
        if (stopped == ERROR_SUCCESS &&
            ChangeServiceConfigW(handles.service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE,
                                 oldBinary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) {
            if (wasRunning)
                StartServiceW(handles.service, 0, nullptr);
            std::error_code ignored;
            fs::remove_all(runtime, ignored);
        } else
            LogError("[ElevationService] Failed to restore previous service runtime");
    } else if (result == ERROR_SUCCESS && !oldRuntime.empty()) {
        const DWORD cleanup = RemoveRuntime(oldRuntime);
        if (cleanup != ERROR_SUCCESS)
            LogWarn("[ElevationService] Previous runtime cleanup incomplete (error=%lu)", cleanup);
    }
    if (result == ERROR_SUCCESS)
        LogInfo("[ElevationService] Service registered in the application folder's protected runtime");
    return result;
}

DWORD RemoveElevationService() {
    if (!ce::elevation::IsElevated())
        return ERROR_ELEVATION_REQUIRED;
    ServiceHandles handles;
    handles.manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!handles.manager)
        return GetLastError();
    handles.service = OpenServiceW(handles.manager, ce::elevation::kServiceName, SERVICE_ALL_ACCESS);
    fs::path runtime;
    if (handles.service) {
        const DWORD valid = ValidateRegistration(handles.service, &runtime);
        if (valid != ERROR_SUCCESS)
            return valid;
        // Prevent a still-connected sensor from restarting the service during removal.
        if (!ChangeServiceConfigW(handles.service, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE, nullptr,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr))
            return GetLastError();
        const DWORD stop = StopService(handles.service);
        if (stop != ERROR_SUCCESS) {
            ChangeServiceConfigW(handles.service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            return stop;
        }
        if (!DeleteService(handles.service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
            const DWORD error = GetLastError();
            ChangeServiceConfigW(handles.service, SERVICE_NO_CHANGE, SERVICE_DEMAND_START, SERVICE_NO_CHANGE, nullptr,
                                 nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            return error;
        }
        CloseServiceHandle(handles.service);
        handles.service = nullptr;
    } else if (GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
        return GetLastError();
    if (!runtime.empty()) {
        const DWORD cleanup = RemoveRuntime(runtime);
        if (cleanup != ERROR_SUCCESS) {
            LogWarn("[ElevationService] Removed service runtime cleanup incomplete (error=%lu)", cleanup);
            return cleanup;
        }
        LogInfo("[ElevationService] Removed registered runtime");
    }
    return WaitElevationServiceRemoved();
}
}  // namespace ce::startup
