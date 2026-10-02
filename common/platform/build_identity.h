#pragma once

#include <cstdint>

uint32_t GetCurrentBuildNumber() noexcept;
const char* GetCaptureVersion() noexcept;
const char* GetBuildTimestamp() noexcept;
