#pragma once

#include <windows.h>
#include <cstdint>

namespace ce::diagnostic_arm {

void CheckAndCache(const char* sessionDir, const char* processName);
bool IsArmed();
void Clear();
void SetForTesting(bool armed, const char* targetProcess = nullptr);
const char* GetTargetProcess();

}  // namespace ce::diagnostic_arm
