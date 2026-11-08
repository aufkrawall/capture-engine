// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#pragma once

#include <windows.h>

#include "module_address_cache.h"

namespace ce::overlay_compat {

inline bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleOut = nullptr) {
    if (moduleOut) {
        *moduleOut = nullptr;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!codeAddress) {
        return false;
    }

    // Served from the loader-free module identity cache once the unload notification is live
    // (see module_address_cache.h); callers on the Present/ECL paths never wait on the loader lock.
    HMODULE callerModule = nullptr;
    bool hasPath = false;
    if (!module_address_cache::Lookup(codeAddress, &callerModule, modulePathOut, modulePathOutCount, &hasPath) ||
        !callerModule) {
        return false;
    }

    if (moduleOut) {
        *moduleOut = callerModule;
    }
    if (modulePathOut && modulePathOutCount > 0 && !hasPath) {
        modulePathOut[0] = '\0';
        return false;
    }

    return true;
}

inline bool IsFFXFrameGenerationModuleHandle(HMODULE moduleHandle, char* modulePathOut = nullptr,
                                             size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!moduleHandle) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, modulePath, _TRUNCATE);
    }
    return IsFFXFrameGenerationModulePath(modulePath);
}

inline bool IsCodeAddressFromFFXFrameGenerationModule(const void* codeAddress, char* modulePathOut = nullptr,
                                                      size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    // One cached resolution yields the path. Asking the loader for it again by handle would put
    // GetModuleFileNameA (loader lock) back on the Present path.
    char modulePath[MAX_PATH] = {};
    HMODULE callerModule = nullptr;
    if (!TryGetModulePathFromCodeAddress(codeAddress, modulePath, sizeof(modulePath), &callerModule) ||
        !callerModule) {
        return false;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, modulePath, _TRUNCATE);
    }
    return IsFFXFrameGenerationModulePath(modulePath);
}

inline bool HasFFXFrameGenerationModuleInStack(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }

    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char candidatePath[MAX_PATH] = {};
        if (IsCodeAddressFromFFXFrameGenerationModule(stackFrames[i], candidatePath, sizeof(candidatePath))) {
            if (modulePathOut && modulePathOutCount > 0) {
                strncpy_s(modulePathOut, modulePathOutCount, candidatePath, _TRUNCATE);
            }
            return true;
        }
    }

    return false;
}

inline bool IsCodeAddressFromStreamlineFrameGenerationModule(const void* codeAddress, char* modulePathOut = nullptr,
                                                             size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    // Classify a local copy, so a caller that passes no buffer still gets a real answer.
    char modulePath[MAX_PATH] = {};
    HMODULE callerModule = nullptr;
    if (!TryGetModulePathFromCodeAddress(codeAddress, modulePath, sizeof(modulePath), &callerModule) ||
        !callerModule) {
        return false;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, modulePath, _TRUNCATE);
    }
    return IsStreamlineFrameGenerationModulePath(modulePath);
}

inline bool HasStreamlineFrameGenerationModuleInStack(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }

    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char candidatePath[MAX_PATH] = {};
        if (IsCodeAddressFromStreamlineFrameGenerationModule(stackFrames[i], candidatePath, sizeof(candidatePath))) {
            if (modulePathOut && modulePathOutCount > 0) {
                strncpy_s(modulePathOut, modulePathOutCount, candidatePath, _TRUNCATE);
            }
            return true;
        }
    }

    return false;
}

inline bool IsPresentInterposerModuleHandle(HMODULE moduleHandle, char* modulePathOut = nullptr,
                                            size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!moduleHandle) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, modulePath, _TRUNCATE);
    }
    return IsPresentInterposerModulePath(modulePath);
}

inline bool IsCodeAddressFromPresentInterposerModule(const void* codeAddress, char* modulePathOut = nullptr,
                                                     size_t modulePathOutCount = 0) {
    HMODULE callerModule = nullptr;
    if (!TryGetModulePathFromCodeAddress(codeAddress, modulePathOut, modulePathOutCount, &callerModule) ||
        !callerModule) {
        return false;
    }

    return IsPresentInterposerModuleHandle(callerModule, modulePathOut, modulePathOutCount);
}

inline bool HasPresentInterposerModuleInStack(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }

    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char candidatePath[MAX_PATH] = {};
        if (IsCodeAddressFromPresentInterposerModule(stackFrames[i], candidatePath, sizeof(candidatePath))) {
            if (modulePathOut && modulePathOutCount > 0) {
                strncpy_s(modulePathOut, modulePathOutCount, candidatePath, _TRUNCATE);
            }
            return true;
        }
    }

    return false;
}

inline bool IsCodeAddressFromXeFGFrameGenerationModule(const void* codeAddress, char* modulePathOut = nullptr,
                                                       size_t modulePathOutCount = 0) {
    char modulePath[MAX_PATH] = {};
    HMODULE callerModule = nullptr;
    if (!TryGetModulePathFromCodeAddress(codeAddress, modulePath, sizeof(modulePath), &callerModule) ||
        !callerModule) {
        if (modulePathOut && modulePathOutCount > 0) {
            modulePathOut[0] = '\0';
        }
        return false;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, modulePath, _TRUNCATE);
    }
    return IsXeFGFrameGenerationModulePath(modulePath);
}

// Module identity of last resort for an XeFG create or present CE saw without its init scope (a late
// injection): libxess_fg.dll on the calling stack.
inline bool HasXeFGFrameGenerationModuleInStack(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    constexpr USHORT kMaxFrames = 24;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        if (IsCodeAddressFromXeFGFrameGenerationModule(stackFrames[i], modulePathOut, modulePathOutCount)) {
            return true;
        }
    }
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    return false;
}

inline const char* GetEffectiveCreateSwapchainCallerModulePath(const char* forwardedCallerModulePath,
                                                               const char* immediateCallerModulePath) {
    if (forwardedCallerModulePath && *forwardedCallerModulePath) {
        return forwardedCallerModulePath;
    }

    return immediateCallerModulePath;
}

inline bool IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(const char* forwardedCallerModulePath,
                                                                  const char* immediateCallerModulePath) {
    return IsThirdPartyOverlayModulePath(
        GetEffectiveCreateSwapchainCallerModulePath(forwardedCallerModulePath, immediateCallerModulePath));
}

}  // namespace ce::overlay_compat
