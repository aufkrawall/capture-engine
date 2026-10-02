// Installed-Apps record, runtime write permissions and the
// "is Capture Engine already installed here" probe.

#include "setup.h"

#include <aclapi.h>

#include <array>
#include <ctime>

namespace ce::setup {
namespace {

bool SetRegistryString(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool SetRegistryDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
           ERROR_SUCCESS;
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* name, REGSAM view) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
        return {};
    std::wstring value(1024, L'\0');
    DWORD type = 0;
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS status =
        RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t))
        return {};
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0')
        value.pop_back();
    return value;
}

// A DWORD, or `fallback` when the value is missing or not a DWORD.
DWORD ReadRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* name, REGSAM view, DWORD fallback) {
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(root, subKey, name, RRF_RT_REG_DWORD | (view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : 0),
                     nullptr, &value, &bytes) != ERROR_SUCCESS)
        return fallback;
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Installed Apps record
// ---------------------------------------------------------------------------

bool WriteUninstallRecord(const std::wstring& directory, uint64_t estimatedBytes, DWORD* error) {
    HKEY key = nullptr;
    const LSTATUS created = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0,
                                            KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        if (error)
            *error = static_cast<DWORD>(created);
        return false;
    }
    const std::wstring exe = JoinPath(directory, kAppExe);
    const std::wstring uninstaller = JoinPath(directory, kUninstallerExe);
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t date[16];
    swprintf(date, 16, L"%04d%02d%02d", now.wYear, now.wMonth, now.wDay);
    bool ok = SetRegistryString(key, L"DisplayName", kProductName) && SetRegistryString(key, L"DisplayVersion", kVersion) &&
              SetRegistryString(key, L"Publisher", kPublisher) && SetRegistryString(key, L"InstallLocation", directory) &&
              SetRegistryString(key, L"DisplayIcon", L"\"" + exe + L"\",0") &&
              SetRegistryString(key, L"UninstallString", L"\"" + uninstaller + L"\"") &&
              SetRegistryString(key, L"QuietUninstallString", L"\"" + uninstaller + L"\" /S") &&
              SetRegistryString(key, L"URLInfoAbout", kHomepage) && SetRegistryString(key, L"InstallDate", date) &&
              SetRegistryDword(key, L"NoModify", 1) && SetRegistryDword(key, L"NoRepair", 1) &&
              SetRegistryDword(key, kOwnRecordMarker, 1) &&
              SetRegistryDword(key, L"EstimatedSize", static_cast<DWORD>(estimatedBytes / 1024));
    if (!ok && error)
        *error = GetLastError();
    RegCloseKey(key);
    return ok;
}

bool RemoveUninstallRecord() {
    HKEY parent = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                                   KEY_ALL_ACCESS | KEY_WOW64_64KEY, &parent);
    if (status == ERROR_FILE_NOT_FOUND)
        return true;
    if (status != ERROR_SUCCESS)
        return false;
    status = RegDeleteTreeW(parent, L"CaptureEngine");
    RegCloseKey(parent);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

// ---------------------------------------------------------------------------
// Permissions
// ---------------------------------------------------------------------------

bool GrantUsersModify(const std::wstring& path, bool inheritToChildren, DWORD* error) {
    std::array<BYTE, SECURITY_MAX_SID_SIZE> sidStorage{};
    DWORD sidSize = static_cast<DWORD>(sidStorage.size());
    if (!CreateWellKnownSid(WinBuiltinUsersSid, nullptr, sidStorage.data(), &sidSize)) {
        if (error)
            *error = GetLastError();
        return false;
    }
    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = inheritToChildren ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sidStorage.data());

    PACL oldAcl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                         &oldAcl, nullptr, &descriptor);
    PACL newAcl = nullptr;
    if (status == ERROR_SUCCESS)
        status = SetEntriesInAclW(1, &access, oldAcl, &newAcl);
    if (status == ERROR_SUCCESS)
        status = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, newAcl, nullptr);
    if (newAcl)
        LocalFree(newAcl);
    if (descriptor)
        LocalFree(descriptor);
    if (status != ERROR_SUCCESS && error)
        *error = status;
    return status == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Existing installation
// ---------------------------------------------------------------------------

ExistingInstall ReadExistingInstall(const std::wstring& candidateDirectory) {
    ExistingInstall existing;
    // Only a record this setup wrote counts: an entry left by another installer
    // under the same key name has a different layout and its folder is not ours to manage.
    if (ReadRegistryDword(HKEY_LOCAL_MACHINE, kUninstallKey, kOwnRecordMarker, KEY_WOW64_64KEY, 0) == 1) {
        existing.directory =
            ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", KEY_WOW64_64KEY);
        existing.version = ReadRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", KEY_WOW64_64KEY);
    }
    existing.registered = !existing.directory.empty();
    const std::wstring directory = existing.registered ? existing.directory : candidateDirectory;
    existing.state.installed = existing.registered || (!directory.empty() && PathExists(JoinPath(directory, kAppExe)));
    existing.state.pawnIoInstalled = IsPawnIoInstalled();
    existing.state.desktopShortcut = OwnedShortcutExists(DesktopShortcutPath(), directory);
    existing.state.startMenuShortcut = OwnedShortcutExists(StartMenuShortcutPath(), directory);

    std::wstring sid;
    bool administrator = false;
    if (ResolveOwner(&sid, &administrator)) {
        // Same record the application writes: three DWORDs (service, elevated, autostart).
        std::array<DWORD, 3> values{};
        DWORD size = sizeof(values);
        const std::wstring key = sid + L"\\Software\\CaptureEngine";
        if (RegGetValueW(HKEY_USERS, key.c_str(), L"StartupPreferences", RRF_RT_REG_BINARY, nullptr, values.data(),
                         &size) == ERROR_SUCCESS &&
            size == sizeof(values) && values[0] <= 1 && values[1] <= 1 && values[2] <= 1) {
            existing.state.hasPreferences = true;
            existing.state.service = values[0] != 0;
            existing.state.elevated = values[1] != 0;
            existing.state.autostart = values[2] != 0;
        }
    }
    if (ElevationServiceExists())
        existing.state.service = true;
    return existing;
}

}  // namespace ce::setup
