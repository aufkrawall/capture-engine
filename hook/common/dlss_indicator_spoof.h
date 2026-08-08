/**
 * DLSS on-screen indicator spoofing (config key `dlss_debug_overlay`).
 *
 * The NVIDIA NGX runtimes decide whether to draw their debug indicator by
 * probing HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator.
 * Both nvngx_dlss.dll (super resolution) and nvngx_dlssg.dll (frame generation)
 * carry that value name and read it through advapi32's wide registry entry
 * points, and both are loaded long after CE's hook thread starts.
 *
 * CE answers the probe process-locally with an inline hook on the shared
 * advapi32 exports, so the setting works without writing to the machine-wide
 * registry and without depending on module load order or import style.
 */

#pragma once

#include <windows.h>

#include <string>

namespace ce::dlss_indicator {

enum class Mode { kPassthrough, kForceOn, kForceOff };

// The value name the NGX runtimes probe, and the payload that turns the
// on-screen indicator on. Anything else (0 here) keeps it hidden.
inline constexpr wchar_t kValueName[] = L"ShowDlssIndicator";
inline constexpr DWORD kIndicatorOn = 0x400;

// "default"/"" -> kPassthrough, "on"/"1"/"true"/"enabled" -> kForceOn,
// "off"/"0"/"false"/"disabled" -> kForceOff. Case- and whitespace-tolerant;
// anything unrecognized stays kPassthrough so a typo never changes behavior.
Mode ParseMode(const std::string& configValue);

const char* GetModeName(Mode mode);

// Outcome of a spoofed registry read, shared by both detours.
struct Answer {
    bool handled = false;  // false: forward the call to the real registry
    LSTATUS status = ERROR_SUCCESS;
    DWORD type = REG_DWORD;
    DWORD value = 0;                       // payload for the caller's buffer
    DWORD requiredBytes = sizeof(DWORD);   // what the caller must report/allocate
    bool writeValue = false;               // buffer is large enough for `value`
};

// RegQueryValueExW-shaped decision. `bufferBytes` is the caller's *lpcbData and
// is ignored when `hasDataBuffer` is false (a size probe).
Answer ResolveQueryValue(Mode mode, const wchar_t* valueName, bool hasDataBuffer, DWORD bufferBytes);

// RegGetValueW-shaped decision; honors the caller's RRF_RT_* type restriction.
Answer ResolveGetValue(Mode mode, const wchar_t* valueName, DWORD rrfFlags, bool hasDataBuffer, DWORD bufferBytes);

// Arms the advapi32 inline hooks that answer the probe. Idempotent, and a
// kPassthrough mode installs nothing so unconfigured processes stay untouched.
// Returns true when the spoof is active afterwards.
bool Install(Mode mode);

// Current mode; kPassthrough until Install() arms a spoof.
Mode GetActiveMode();

}  // namespace ce::dlss_indicator
