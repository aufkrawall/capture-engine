#include "startup_preferences.h"
#include "common/ipc/elevation_windows.h"

namespace ce::startup {
namespace {
std::wstring ownerSid;

std::wstring PreferenceKey() {
    return OwnerSid() + L"\\Software\\CaptureEngine";
}

}  // namespace

std::wstring OwnerSid() {
    if (!ownerSid.empty())
        return ownerSid;
    wchar_t inherited[256]{};
    const DWORD size = GetEnvironmentVariableW(kOwnerEnvironment, inherited, 256);
    PSID sid = nullptr;
    if (size != 0 && size < 256 && ConvertStringSidToSidW(inherited, &sid)) {
        LocalFree(sid);
        return inherited;
    }
    return ce::elevation::ProcessUserSid(GetCurrentProcess());
}

void SetOwnerSid(const std::wstring& sid) {
    ownerSid = sid;
    SetEnvironmentVariableW(kOwnerEnvironment, sid.c_str());
}

Preferences ReadPreferences() {
    std::array<DWORD, 3> values{};
    DWORD size = sizeof(values);
    if (RegGetValueW(HKEY_USERS, PreferenceKey().c_str(), L"StartupPreferences", RRF_RT_REG_BINARY, nullptr,
                     values.data(), &size) != ERROR_SUCCESS ||
        size != sizeof(values))
        return {};
    for (DWORD value : values) {
        if (value > 1)
            return {};
    }
    return {values[0] != 0, values[1] != 0, values[2] != 0};
}

bool WritePreferences(const Preferences& preferences) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_USERS, PreferenceKey().c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const DWORD service = preferences.service;
    const DWORD elevated = preferences.elevated;
    const DWORD autostart = preferences.autostart;
    // One binary record is the commit marker; readers never observe half a toggle.
    const std::array<DWORD, 3> values{service, elevated, autostart};
    const LSTATUS status = RegSetValueExW(key, L"StartupPreferences", 0, REG_BINARY,
                                          reinterpret_cast<const BYTE*>(values.data()), sizeof(values));
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}
}  // namespace ce::startup
