#pragma once

#include <cstddef>

namespace ce::nvngx_parameter_abi {

// NVSDK_NGX_Parameter virtual ABI. Slots 8/9 are pointer/resource getters;
// treating them as GetI/GetUI corrupts caller memory.
inline constexpr size_t kSetI = 3;
inline constexpr size_t kSetUI = 4;
inline constexpr size_t kSetF = 6;
inline constexpr size_t kGetI = 11;
inline constexpr size_t kGetUI = 12;

}  // namespace ce::nvngx_parameter_abi
