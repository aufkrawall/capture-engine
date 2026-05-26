#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

// FFX Hook - Hooks AMD FidelityFX API to detect FSR Frame Generation activation
// This provides usage-based detection (vs DLL-based) by detecting when FG
// context is created

namespace FFXHook {

// Initialize FFX hooks when FidelityFX DLLs are detected
// Should be called from LoadLibrary hook when detecting:
//   - amd_fidelityfx_fg.dll
//   - ffx_frameinterpolation_x64.dll
void Init();

// Register FFX exports with the process-wide GetProcAddress router.
// This is intentionally separate from Init() so startup can arm the router
// before any game thread has a chance to cache an unwrapped ffxConfigure.
void RegisterDynamicHooks();

// Install FFX present callback bridge on all tracked FG contexts.
// Called when FSR FG activates without a direct ffxConfigure interception
// (e.g. via Streamline authoritative takeover).  Returns true if bridge
// was installed on at least one context.
bool InstallBridgeOnTrackedContexts(void* swapChain);

// Check if hooks are already installed
bool IsInitialized();

// Cleanup hooks (called during shutdown)
void Shutdown();

// FFX present-callback bridge storage uses a stable context key per configure call.
void* GetPresentCallbackBridgeKey(void* context);

namespace detail {

enum class InlineDetourProbeState {
    kInstalledExpected,
    kMissingOrChanged,
    kUnreadableTarget,
};

struct InlineDetourProbeResult {
    InlineDetourProbeState state = InlineDetourProbeState::kMissingOrChanged;
    DWORD win32Error = ERROR_SUCCESS;
};

inline bool SnapshotMatchesExpectedInlineDetour(const void* target, const unsigned char* code, size_t codeSize,
                                                const void* detour) {
    if (!target || !code || !detour) {
        return false;
    }

#ifdef _WIN64
    if (codeSize < 14 || code[0] != 0xFF || code[1] != 0x25 || code[2] != 0x00 || code[3] != 0x00 || code[4] != 0x00 ||
        code[5] != 0x00) {
        return false;
    }

    void* installedDetour = nullptr;
    std::memcpy(&installedDetour, code + 6, sizeof(installedDetour));
    return installedDetour == detour;
#else
    if (codeSize < 5 || code[0] != 0xE9) {
        return false;
    }

    int32_t relativeTarget = 0;
    std::memcpy(&relativeTarget, code + 1, sizeof(relativeTarget));
    const auto* installedDetour = reinterpret_cast<const std::uint8_t*>(target) + 5 + relativeTarget;
    return installedDetour == detour;
#endif
}

inline InlineDetourProbeResult ProbeExpectedInlineDetourInstalled(const void* target, const void* detour) {
    InlineDetourProbeResult result{};
    if (!target || !detour) {
        return result;
    }

#ifdef _WIN64
    unsigned char snapshot[14] = {};
#else
    unsigned char snapshot[5] = {};
#endif

    SIZE_T bytesRead = 0;
    SetLastError(ERROR_SUCCESS);
    const BOOL readOk = ReadProcessMemory(GetCurrentProcess(), target, snapshot, sizeof(snapshot), &bytesRead);
    if (!readOk || bytesRead != sizeof(snapshot)) {
        result.state = InlineDetourProbeState::kUnreadableTarget;
        result.win32Error = readOk ? ERROR_PARTIAL_COPY : GetLastError();
        return result;
    }

    result.state = SnapshotMatchesExpectedInlineDetour(target, snapshot, sizeof(snapshot), detour)
                       ? InlineDetourProbeState::kInstalledExpected
                       : InlineDetourProbeState::kMissingOrChanged;
    return result;
}

inline bool IsEntryBreakpointHit(const void* exceptionAddress, uintptr_t instructionPointer, const void* target) {
    if (!target) {
        return false;
    }

    const uintptr_t targetAddress = reinterpret_cast<uintptr_t>(target);
    const uintptr_t exceptionAddressValue = reinterpret_cast<uintptr_t>(exceptionAddress);

    // Windows reports int3 breakpoints with the exception address at the
    // patched byte, while the context instruction pointer is commonly already
    // advanced past that one-byte instruction. Accept both forms so a protected
    // runtime cannot escape the bridge as an unhandled STATUS_BREAKPOINT.
    return exceptionAddressValue == targetAddress || exceptionAddressValue == targetAddress + 1 ||
           instructionPointer == targetAddress || instructionPointer == targetAddress + 1;
}

}  // namespace detail

}  // namespace FFXHook
