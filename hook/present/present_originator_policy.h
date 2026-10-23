#pragma once

// Who issued a Present, from the frames above CE's hook. Header-only so the unit tests drive it directly.
//
// Below a foreign Present chain (Steam, RTSS) the immediate caller is always the last foreign overlay, so the
// originator is the first frame above CE's own frames, the overlays and the graphics dispatch modules. That
// frame decides which frame-generation runtime, if any, presented: the overlay routing and the FG status read it.

#include <cstddef>
#include <cstdint>

#include "hook/overlay/overlay_compat_detail/module_table.h"

namespace ce::present_originator {

enum class FrameKind : uint8_t {
    kNoImage,            // a trampoline or thunk outside every module: keep walking
    kCaptureEngine,      // CE's own detours and wrappers
    kThirdPartyOverlay,  // the chain CE sits below
    kGraphicsDispatch,   // the system's DXGI/D3D dispatch modules between the overlays
    kOther,              // an originator candidate
};

enum class Originator : uint8_t {
    kUnresolved,          // no candidate frame (all frames CE, overlays or dispatch)
    kApplication,         // anything that is not a frame-generation runtime
    kStreamline,          // sl.interposer / sl.common / sl.dlss_g / nvngx_dlssg
    kFFXFrameGeneration,  // AMD FidelityFX frame generation
    kXeFG,                // Intel XeSS frame generation (libxess_fg.dll)
    kPresentInterposer,   // NVIDIA Smooth Motion (NvPresent64/32)
};

inline const char* OriginatorName(Originator originator) {
    switch (originator) {
        case Originator::kApplication:
            return "application";
        case Originator::kStreamline:
            return "streamline";
        case Originator::kFFXFrameGeneration:
            return "ffx";
        case Originator::kXeFG:
            return "xefg";
        case Originator::kPresentInterposer:
            return "interposer";
        case Originator::kUnresolved:
        default:
            return "unresolved";
    }
}

// The DXGI/D3D dispatch modules that can sit between two overlays in a Present chain, and only the system's own
// copies: a same-named module elsewhere is a proxy (OptiScaler's game-folder dxgi.dll, a translation layer), and
// a proxy is code of its own - it is classified as itself, never stepped over by its base name
// (multi-overlay-present-entry-rule). The Agility SDK ships D3D12Core.dll and D3D12SDKLayers.dll beside the game
// under their system names; they are Microsoft's runtime wherever they load from.
// `systemDirectory` is GetSystemDirectory() (SysWOW64 in a 32-bit process), without a trailing separator.
inline bool IsGraphicsDispatchModulePath(const char* modulePath, const char* systemDirectory) {
    if (!modulePath || !modulePath[0]) {
        return false;
    }
    const char* baseName = ce::overlay_compat::ExtractModuleBaseName(modulePath);
    static const char* const kAgilityModules[] = {"d3d12core.dll", "d3d12sdklayers.dll"};
    for (const char* candidate : kAgilityModules) {
        if (ce::overlay_compat::detail::EqualsInsensitive(baseName, candidate)) {
            return true;
        }
    }
    static const char* const kSystemDispatchModules[] = {"dxgi.dll",  "d3d12.dll", "d3d11.dll",
                                                         "d3d11on12.dll", "d3d10.dll", "d3d10_1.dll",
                                                         "d3d9.dll",  "dcomp.dll", "dwmapi.dll"};
    bool dispatchName = false;
    for (const char* candidate : kSystemDispatchModules) {
        if (ce::overlay_compat::detail::EqualsInsensitive(baseName, candidate)) {
            dispatchName = true;
            break;
        }
    }
    if (!dispatchName || !systemDirectory || !systemDirectory[0] || baseName == modulePath) {
        return false;
    }
    // The module's directory, compared without its trailing separator.
    const size_t directoryLength = static_cast<size_t>(baseName - modulePath) - 1;
    size_t systemLength = 0;
    while (systemDirectory[systemLength]) {
        ++systemLength;
    }
    while (systemLength > 0 && (systemDirectory[systemLength - 1] == '\\' || systemDirectory[systemLength - 1] == '/')) {
        --systemLength;
    }
    if (directoryLength != systemLength) {
        return false;
    }
    for (size_t i = 0; i < systemLength; ++i) {
        const char a = modulePath[i] == '/' ? '\\' : modulePath[i];
        const char b = systemDirectory[i] == '/' ? '\\' : systemDirectory[i];
        if (ce::overlay_compat::detail::ToLowerAscii(a) != ce::overlay_compat::detail::ToLowerAscii(b)) {
            return false;
        }
    }
    return true;
}

// One stack frame's module, in the walk's order of precedence. An FG runtime's own module is never an overlay
// (IsThirdPartyOverlayModulePath), so libxess_fg.dll in OptiScaler's folder is a candidate, not skipped.
inline FrameKind ClassifyFrame(const char* modulePath, bool captureEngineModule, const char* systemDirectory) {
    if (!modulePath || !modulePath[0]) {
        return FrameKind::kNoImage;
    }
    if (captureEngineModule) {
        return FrameKind::kCaptureEngine;
    }
    if (ce::overlay_compat::IsThirdPartyOverlayModulePath(modulePath)) {
        return FrameKind::kThirdPartyOverlay;
    }
    if (IsGraphicsDispatchModulePath(modulePath, systemDirectory)) {
        return FrameKind::kGraphicsDispatch;
    }
    return FrameKind::kOther;
}

// The first candidate frame decides; frames above it are its own callers. -1 when there is none.
inline int SelectOriginatorFrame(const FrameKind* kinds, int count) {
    if (!kinds) {
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        if (kinds[i] == FrameKind::kOther) {
            return i;
        }
    }
    return -1;
}

// What the originating module is. XeFG first: its module identity is exact (base name) and must win over any
// other classification of the folder it lives in.
inline Originator ClassifyOriginatorModule(const char* modulePath, bool streamlineModule) {
    if (!modulePath || !modulePath[0]) {
        return Originator::kUnresolved;
    }
    if (ce::overlay_compat::IsXeFGFrameGenerationModulePath(modulePath)) {
        return Originator::kXeFG;
    }
    if (streamlineModule || ce::overlay_compat::IsStreamlineFrameGenerationModulePath(modulePath)) {
        return Originator::kStreamline;
    }
    if (ce::overlay_compat::IsFFXFrameGenerationModulePath(modulePath)) {
        return Originator::kFFXFrameGeneration;
    }
    if (ce::overlay_compat::IsPresentInterposerModulePath(modulePath)) {
        return Originator::kPresentInterposer;
    }
    return Originator::kApplication;
}

}  // namespace ce::present_originator
