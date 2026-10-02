#pragma once

#include "common/setup/startup_policy.h"
#include <windows.h>
#include <string>

namespace ce::startup {
inline constexpr wchar_t kOwnerEnvironment[] = L"CE_STARTUP_OWNER_SID";
std::wstring OwnerSid();
void SetOwnerSid(const std::wstring& sid);
Preferences ReadPreferences();
bool WritePreferences(const Preferences& preferences);
}  // namespace ce::startup
