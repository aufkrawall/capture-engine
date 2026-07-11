#pragma once

#include <d3d11.h>
#include <windows.h>
#include <cstdint>
#include <string>

#include "../common/gpu_scheduling_policy.h"

namespace ce::windows_gpu_scheduling {

struct AdapterSchedulingEnvironment {
    LUID luid{};
    std::wstring description;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t driverVersion = 0;
    uint32_t windowsBuild = 0;
    ce::gpu_scheduling::HagsStatus hags;
    LONG openStatus = 0;
    LONG caps27Status = 0;
    LONG caps29Status = 0;
    LONG closeStatus = 0;
};

bool GetAdapterLuid(ID3D11Device* device, LUID& luid);
bool QueryAdapterSchedulingEnvironment(const LUID& luid, AdapterSchedulingEnvironment& environment);
bool QueryAdapterSchedulingEnvironment(ID3D11Device* device, AdapterSchedulingEnvironment& environment);
bool SameLuid(const LUID& lhs, const LUID& rhs);
std::string FormatLuid(const LUID& luid);

}  // namespace ce::windows_gpu_scheduling
