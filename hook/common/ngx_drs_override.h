/**
 * Runtime side of the process-local DLSS driver-settings answers.
 *
 * The value encodings, the driver identifiers, the NVDRS_SETTING ABI mirror and
 * every decision that can be expressed as a pure function live in
 * ngx_drs_override_policy.h. This header owns what cannot: the process-wide
 * configured state, the NvAPI_DRS_GetSetting detour, and the
 * `nvapi_QueryInterface` participation that hands that detour out.
 */

#pragma once

#include "ngx_drs_override_policy.h"

namespace ce::ngx_drs {

// Process-wide configured answers. Set from the resolved graphics config;
// safe to call repeatedly, and logs only on an actual change.
void SetConfiguredOverrides(const DlssDrsOverrides& overrides);
DlssDrsOverrides GetConfiguredOverrides();
bool IsArmed();

// Kept as a distinct accessor because the render preset is the one key whose
// consumer (nvngx_dlssg) is not a Streamline module, so the arming path logs it
// separately.
uint32_t GetConfiguredPreset();

// `nvapi_QueryInterface` participation. Returns CE's NvAPI_DRS_GetSetting
// wrapper when this resolution must be intercepted, or nullptr to hand the
// caller the pointer the driver returned.
void* MaybeWrapQueryInterface(uint32_t functionId, void* resolved, const void* callerAddress);

// True when the module at `modulePath` reads the DLSS driver settings and CE
// therefore has to patch its GetProcAddress import before its first
// resolution. `module` may be null, in which case only the name is consulted.
bool IsDlssDrsConsumerModuleLoaded(const char* modulePath, void* module);

}  // namespace ce::ngx_drs
