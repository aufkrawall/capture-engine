#include "dlss_indicator_spoof.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <cwchar>

#include "../wrappers/inline_hook.h"
#include "hook_common.h"

namespace ce::dlss_indicator {

namespace {

using RegQueryValueExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegGetValueW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

std::atomic<Mode> g_mode{Mode::kPassthrough};
std::atomic<bool> g_installed{false};
std::atomic<RegQueryValueExW_t> g_origRegQueryValueExW{nullptr};
std::atomic<RegGetValueW_t> g_origRegGetValueW{nullptr};
std::atomic<unsigned> g_interceptCount{0};
std::atomic<bool> g_missingOriginalLogged{false};

bool MatchesValueName(const wchar_t* valueName) {
    return valueName && _wcsicmp(valueName, kValueName) == 0;
}

// The probe fires a handful of times per feature init, so log the first few and
// then only rarely - enough to prove the spoof answered without hot-path noise.
bool ShouldLogIntercept() {
    const unsigned count = g_interceptCount.fetch_add(1, std::memory_order_relaxed) + 1;
    return count <= 4 || (count % 500) == 0;
}

Answer MakeAnswer(Mode mode, bool hasDataBuffer, DWORD bufferBytes) {
    Answer answer;
    answer.handled = true;
    answer.status = ERROR_SUCCESS;
    answer.type = REG_DWORD;
    answer.value = (mode == Mode::kForceOn) ? kIndicatorOn : 0u;
    answer.requiredBytes = sizeof(DWORD);
    if (!hasDataBuffer)
        return answer;  // size probe: report the type and size only
    if (bufferBytes < sizeof(DWORD)) {
        answer.status = ERROR_MORE_DATA;
        return answer;
    }
    answer.writeValue = true;
    return answer;
}

void PublishRegQueryValueExWTrampoline(void* trampoline, void* /*context*/) {
    g_origRegQueryValueExW.store(reinterpret_cast<RegQueryValueExW_t>(trampoline), std::memory_order_release);
}

void PublishRegGetValueWTrampoline(void* trampoline, void* /*context*/) {
    g_origRegGetValueW.store(reinterpret_cast<RegGetValueW_t>(trampoline), std::memory_order_release);
}

void LogMissingOriginalOnce(const char* api) {
    if (!g_missingOriginalLogged.exchange(true, std::memory_order_relaxed))
        HookLogImportant("DLSS indicator: %s detour ran without a trampoline - reporting value-not-found", api);
}

LSTATUS WINAPI DetourRegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData,
                                      LPDWORD lpcbData) {
    // A data buffer without a size is an invalid call; leave it to the real API.
    if (lpData == nullptr || lpcbData != nullptr) {
        const Mode mode = g_mode.load(std::memory_order_acquire);
        const Answer answer =
            ResolveQueryValue(mode, lpValueName, lpData != nullptr, lpcbData ? *lpcbData : 0u);
        if (answer.handled) {
            if (lpType)
                *lpType = answer.type;
            if (answer.writeValue && lpData)
                std::memcpy(lpData, &answer.value, sizeof(DWORD));
            if (lpcbData)
                *lpcbData = answer.requiredBytes;
            if (ShouldLogIntercept()) {
                HookLogImportant("DLSS indicator: answered RegQueryValueExW(%ls) mode=%s value=0x%X status=%ld",
                                 kValueName, GetModeName(mode), answer.value, static_cast<long>(answer.status));
            }
            return answer.status;
        }
    }

    const RegQueryValueExW_t original = g_origRegQueryValueExW.load(std::memory_order_acquire);
    if (!original) {
        LogMissingOriginalOnce("RegQueryValueExW");
        return ERROR_FILE_NOT_FOUND;
    }
    return original(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

LSTATUS WINAPI DetourRegGetValueW(HKEY hkey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags, LPDWORD pdwType,
                                  PVOID pvData, LPDWORD pcbData) {
    if (pvData == nullptr || pcbData != nullptr) {
        const Mode mode = g_mode.load(std::memory_order_acquire);
        const Answer answer =
            ResolveGetValue(mode, lpValue, dwFlags, pvData != nullptr, pcbData ? *pcbData : 0u);
        if (answer.handled) {
            if (pdwType)
                *pdwType = answer.type;
            if (answer.writeValue && pvData)
                std::memcpy(pvData, &answer.value, sizeof(DWORD));
            if (pcbData)
                *pcbData = answer.requiredBytes;
            if (ShouldLogIntercept()) {
                HookLogImportant("DLSS indicator: answered RegGetValueW(%ls) mode=%s value=0x%X status=%ld flags=0x%X",
                                 kValueName, GetModeName(mode), answer.value, static_cast<long>(answer.status),
                                 dwFlags);
            }
            return answer.status;
        }
    }

    const RegGetValueW_t original = g_origRegGetValueW.load(std::memory_order_acquire);
    if (!original) {
        LogMissingOriginalOnce("RegGetValueW");
        return ERROR_FILE_NOT_FOUND;
    }
    return original(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
}

// advapi32's registry exports are 7-byte JMP thunks into kernelbase, which is a
// poor inline-hook target. Hook the shared implementation instead: it has an
// ordinary prologue to trampoline, it is what advapi32's thunk lands in, and it
// also covers modules that import through the api-ms-win-core-registry api set.
// advapi32 stays as a fallback in case a host resolves the export elsewhere.
constexpr const wchar_t* kRegistryHostModules[] = {L"kernelbase.dll", L"advapi32.dll"};

// Publishes the trampoline before the target goes live so a concurrent registry
// read from another thread can never observe a detour without an original.
bool InstallOne(const char* exportName, void* detour, InlineHook::TrampolinePublisher publisher) {
    for (const wchar_t* moduleName : kRegistryHostModules) {
        HMODULE module = GetModuleHandleW(moduleName);
        if (!module)
            module = LoadLibraryW(moduleName);
        if (!module)
            continue;
        void* target = reinterpret_cast<void*>(GetProcAddress(module, exportName));
        if (!target)
            continue;
        void* trampoline = nullptr;
        if (InlineHook::InstallPublished(target, detour, &trampoline, publisher, nullptr)) {
            HookLog("DLSS indicator: hooked %ls!%s at %p", moduleName, exportName, target);
            return true;
        }
        HookLogImportant("DLSS indicator: failed to hook %ls!%s at %p", moduleName, exportName, target);
    }
    HookLogImportant("DLSS indicator: no registry host module exposed a hookable %s", exportName);
    return false;
}

}  // namespace

Mode ParseMode(const std::string& configValue) {
    std::string value;
    value.reserve(configValue.size());
    for (unsigned char ch : configValue) {
        if (ch == ' ' || ch == '\t')
            continue;
        value.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (value == "on" || value == "1" || value == "true" || value == "yes" || value == "enabled")
        return Mode::kForceOn;
    if (value == "off" || value == "0" || value == "false" || value == "no" || value == "disabled")
        return Mode::kForceOff;
    return Mode::kPassthrough;
}

const char* GetModeName(Mode mode) {
    switch (mode) {
        case Mode::kForceOn:
            return "on";
        case Mode::kForceOff:
            return "off";
        default:
            return "default";
    }
}

Answer ResolveQueryValue(Mode mode, const wchar_t* valueName, bool hasDataBuffer, DWORD bufferBytes) {
    if (mode == Mode::kPassthrough || !MatchesValueName(valueName))
        return Answer{};
    return MakeAnswer(mode, hasDataBuffer, bufferBytes);
}

Answer ResolveGetValue(Mode mode, const wchar_t* valueName, DWORD rrfFlags, bool hasDataBuffer, DWORD bufferBytes) {
    if (mode == Mode::kPassthrough || !MatchesValueName(valueName))
        return Answer{};

    // RegGetValueW rejects a value whose type the caller did not ask for. We
    // only ever synthesize a REG_DWORD, so mirror that rejection rather than
    // handing back a type the caller explicitly excluded.
    const DWORD typeRestriction = rrfFlags & RRF_RT_ANY;
    if (typeRestriction != 0 && (typeRestriction & RRF_RT_REG_DWORD) == 0) {
        Answer answer;
        answer.handled = true;
        answer.status = ERROR_UNSUPPORTED_TYPE;
        answer.type = REG_DWORD;
        answer.requiredBytes = sizeof(DWORD);
        return answer;
    }
    return MakeAnswer(mode, hasDataBuffer, bufferBytes);
}

Mode GetActiveMode() {
    return g_mode.load(std::memory_order_acquire);
}

bool Install(Mode mode) {
    if (mode == Mode::kPassthrough) {
        HookLog("DLSS indicator: dlss_debug_overlay=default - leaving the NGX registry probe untouched");
        return false;
    }
    if (g_installed.load(std::memory_order_acquire)) {
        g_mode.store(mode, std::memory_order_release);
        return true;
    }

    // Arm the mode before the hooks go live so the first probe already sees it.
    g_mode.store(mode, std::memory_order_release);

    const bool queryHooked =
        InstallOne("RegQueryValueExW", reinterpret_cast<void*>(&DetourRegQueryValueExW),
                   &PublishRegQueryValueExWTrampoline);
    const bool getHooked =
        InstallOne("RegGetValueW", reinterpret_cast<void*>(&DetourRegGetValueW), &PublishRegGetValueWTrampoline);

    // nvngx_dlss.dll and nvngx_dlssg.dll read the value through RegQueryValueExW,
    // so that one is the load-bearing hook; RegGetValueW only widens coverage.
    if (!queryHooked && !getHooked) {
        g_mode.store(Mode::kPassthrough, std::memory_order_release);
        HookLogImportant("DLSS indicator: no registry entry point could be hooked - dlss_debug_overlay=%s "
                         "will not take effect",
                         GetModeName(mode));
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    HookLogImportant("DLSS indicator: dlss_debug_overlay=%s armed (RegQueryValueExW=%d RegGetValueW=%d); NGX probes "
                     "for ShowDlssIndicator are answered in-process",
                     GetModeName(mode), queryHooked ? 1 : 0, getHooked ? 1 : 0);
    return true;
}

}  // namespace ce::dlss_indicator
