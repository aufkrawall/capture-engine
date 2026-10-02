#pragma once

// NGX over-the-air (OTA) policy and runtime-override outcome.
//
// Split out of abi_constants_and_config.h: the two `SharedGraphicsConfig`
// bytes stay with the struct they live in, but the values those bytes carry,
// and the reasons the hook publishes back when a configured runtime override
// does not apply, are their own semantic unit and were pushing that file past
// the 800-line ceiling.
//
// Included between abi_constants_and_config.h and shared_memory_layout.h,
// because SharedMemoryLayout::RuntimeOverrideStatus reads the refusal values
// declared here.

#include <cstdint>

// `ngx_ota` policy values. `Default` leaves every NGX/Streamline OTA decision
// to the driver, which is the shipped behaviour.
inline constexpr uint8_t kNgxOtaModeDefault = 0;
// Forced off: refuse the `nvngx_update.exe` launch, publish
// `__NGX_DISABLE_UPDATER`, and clear Streamline's own OTA preference bits so a
// downloaded plugin cannot supersede the staged set.
inline constexpr uint8_t kNgxOtaModeOff = 1;
// Forced on: never refuse the launch, clear an inherited
// `__NGX_DISABLE_UPDATER`, and stand CE's own nvngx_*/sl.* path overrides down
// so the driver's OTA files under %ProgramData%\NVIDIA\NGX\models are the ones
// that actually load.
inline constexpr uint8_t kNgxOtaModeOn = 2;

inline constexpr bool IsNgxOtaMode(uint8_t value) {
    return value <= kNgxOtaModeOn;
}

// `ngx_log` values. `Default` never writes the environment, so NGX keeps
// whatever the driver and the game already decided.
inline constexpr uint8_t kNgxLogLevelDefault = 0;
inline constexpr uint8_t kNgxLogLevelOff = 1;
inline constexpr uint8_t kNgxLogLevelOn = 2;
inline constexpr uint8_t kNgxLogLevelVerbose = 3;

inline constexpr bool IsNgxLogLevel(uint8_t value) {
    return value <= kNgxLogLevelVerbose;
}

// The value CE publishes as `__NGX_LOG_LEVEL` for a requested level. NGX's own
// scale is 0=off, 1=on, 2=verbose; `Default` has no representation because it
// means "do not write the variable at all".
inline constexpr const char* NgxLogLevelEnvironmentValue(uint8_t level) {
    return level == kNgxLogLevelOff       ? "0"
           : level == kNgxLogLevelOn      ? "1"
           : level == kNgxLogLevelVerbose ? "2"
                                          : nullptr;
}

// Why the hook refused a configured runtime-override path. Published through
// SharedMemoryLayout::runtimeOverrideStatus so the host can tell the user their
// configuration did not take effect instead of leaving it in hook_debug.log.
inline constexpr uint32_t kRuntimeOverrideRefusalNone = 0;
// The Streamline runtime resolved sl.common from somewhere other than the
// configured override, so redirecting the remaining plugins would build a
// version-mixed stack.
inline constexpr uint32_t kRuntimeOverrideRefusalForeignStreamlineCore = 1;
// The configured runtime speaks the other Streamline generation (1.x vs 2.x).
inline constexpr uint32_t kRuntimeOverrideRefusalGenerationMismatch = 2;
// Honouring the redirect would map a second instance of a process-global
// runtime that is already loaded from a different file.
inline constexpr uint32_t kRuntimeOverrideRefusalDuplicateModule = 3;
// `ngx_ota=on` deliberately stands the configured overrides down so the
// driver's OTA files are the ones that load.
inline constexpr uint32_t kRuntimeOverrideRefusalNgxOtaForcedOn = 4;

inline constexpr const char* RuntimeOverrideRefusalText(uint32_t reason) {
    return reason == kRuntimeOverrideRefusalForeignStreamlineCore
               ? "the game's Streamline runtime had already resolved its core (sl.common) from another location, "
                 "so redirecting the remaining plugins would build a version-mixed Streamline stack"
           : reason == kRuntimeOverrideRefusalGenerationMismatch
               ? "the configured runtime speaks a different Streamline generation than this process uses"
           : reason == kRuntimeOverrideRefusalDuplicateModule
               ? "the module was already loaded from a different file, so redirecting would map a second instance "
                 "of a process-global runtime"
           : reason == kRuntimeOverrideRefusalNgxOtaForcedOn
               ? "ngx_ota=on stands the configured DLL overrides down on purpose so the driver's OTA files load"
               : "";
}
