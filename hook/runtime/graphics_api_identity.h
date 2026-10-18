#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace ce::graphics_api_identity {

enum class DirectDrawVersion : unsigned char {
    Unknown = 0,
    DirectDraw = 1,
    DirectDraw2 = 2,
    DirectDraw3 = 3,
    DirectDraw4 = 4,
    DirectDraw7 = 7,
};

enum class OpenGLProfile : unsigned char {
    Unknown = 0,
    Core,
    Compatibility,
};

struct OpenGLIdentity {
    int major = 0;
    int minor = 0;
    OpenGLProfile profile = OpenGLProfile::Unknown;
    bool valid = false;
};

template <typename Identity>
class ScopedIdentityRegistry {
public:
    bool Ensure(const void* scope, const Identity& identity) {
        if (!scope)
            return false;
        return identities.emplace(scope, identity).second;
    }

    bool Set(const void* scope, const Identity& identity) {
        if (!scope)
            return false;
        const auto it = identities.find(scope);
        const bool changed = it == identities.end() || it->second != identity;
        identities[scope] = identity;
        return changed;
    }

    bool Promote(const void* scope, const Identity& identity) {
        if (!scope)
            return false;
        Identity& current = identities[scope];
        if (identity <= current)
            return false;
        current = identity;
        return true;
    }

    bool TryGet(const void* scope, Identity* identity) const {
        if (!scope || !identity)
            return false;
        const auto it = identities.find(scope);
        if (it == identities.end())
            return false;
        *identity = it->second;
        return true;
    }

    void Erase(const void* scope) {
        identities.erase(scope);
    }

private:
    std::unordered_map<const void*, Identity> identities;
};

inline const char* DirectDrawLabel(DirectDrawVersion version) {
    switch (version) {
        case DirectDrawVersion::DirectDraw:
            return "DirectDraw";
        case DirectDrawVersion::DirectDraw2:
            return "DirectDraw2";
        case DirectDrawVersion::DirectDraw3:
            return "DirectDraw3";
        case DirectDrawVersion::DirectDraw4:
            return "DirectDraw4";
        case DirectDrawVersion::DirectDraw7:
            return "DirectDraw7";
        default:
            return "DirectDraw";
    }
}

inline const char* LegacyDirectXLabel(DirectDrawVersion directDrawVersion, unsigned direct3DVersion) {
    if (direct3DVersion == 7)
        return "DX7";
    if (direct3DVersion == 6)
        return "DX6";
    return DirectDrawLabel(directDrawVersion);
}

inline const char* D3D9Label(bool isEx, bool isDxvk) {
    if (isEx)
        return isDxvk ? "DX9Ex (DXVK)" : "DX9Ex";
    return isDxvk ? "DX9 (DXVK)" : "DX9";
}

inline const char* D3D10Label(bool is10_1, bool isDxvk) {
    if (is10_1)
        return isDxvk ? "DX10.1 (DXVK)" : "DX10.1";
    return isDxvk ? "DX10 (DXVK)" : "DX10";
}

// Who asked a D3D11 device or context for a newer interface (ID3D11Device1-5, ID3D11DeviceContext1-4).
// Only the application's own request says which D3D11.x it uses: CE's own queries (the prerender
// fence path asks for ID3D11Device5), the runtime implementing the device (Windows' or DXVK's
// d3d11.dll), Windows modules (dxgi, d3d10core, d2d1) and other overlays ask for their own reasons
// and once made every such game "DX11.4" or "DX11.1".
enum class ApiEvidenceCaller : unsigned char {
    kApplication,
    kCaptureEngine,
    kDeviceRuntime,
    kWindows,
    kThirdPartyOverlay,
    kUnresolved,  // no module: a hook thunk or generated code
};

inline ApiEvidenceCaller ClassifyApiEvidenceCaller(bool resolved, bool captureEngine, bool deviceRuntimeModule,
                                                   bool windowsModule, bool thirdPartyOverlay) {
    if (!resolved)
        return ApiEvidenceCaller::kUnresolved;
    if (captureEngine)
        return ApiEvidenceCaller::kCaptureEngine;
    if (deviceRuntimeModule)
        return ApiEvidenceCaller::kDeviceRuntime;
    if (windowsModule)
        return ApiEvidenceCaller::kWindows;
    if (thirdPartyOverlay)
        return ApiEvidenceCaller::kThirdPartyOverlay;
    return ApiEvidenceCaller::kApplication;
}

inline const char* ApiEvidenceCallerName(ApiEvidenceCaller caller) {
    switch (caller) {
        case ApiEvidenceCaller::kApplication:
            return "application";
        case ApiEvidenceCaller::kCaptureEngine:
            return "capture_engine";
        case ApiEvidenceCaller::kDeviceRuntime:
            return "device_runtime";
        case ApiEvidenceCaller::kWindows:
            return "windows";
        case ApiEvidenceCaller::kThirdPartyOverlay:
            return "third_party_overlay";
        default:
            return "unresolved";
    }
}

// Whether `path` (ANSI, as the module cache stores it) lies under `directory`.
inline bool IsPathUnderDirectory(const char* path, const char* directory) {
    if (!path || !directory || !*directory)
        return false;
    const size_t length = std::strlen(directory);
    return _strnicmp(path, directory, length) == 0 && (path[length] == '\\' || path[length] == '/');
}

// The return address of the current function, for ApiEvidenceCaller attribution.
#if defined(__GNUC__) || defined(__clang__)
#define CE_API_EVIDENCE_CALLER() __builtin_extract_return_addr(__builtin_return_address(0))
#else
#include <intrin.h>
#define CE_API_EVIDENCE_CALLER() _ReturnAddress()
#endif

// A D3D12 swapchain's label. vkd3d-proton's d3d12.dll names itself in its version resource (the
// check the Vulkan layer's label uses, "DX12 (VKD3D-Proton)"); Wine's vkd3d is libvkd3d-1.dll behind
// d3d12core.dll. Until 2026-10-18 the D3D12 overlay knew only the Wine form and one of its paths
// always said "DX12".
inline const char* D3D12Label(bool vkd3dProtonD3D12, bool wineVkd3d) {
    if (vkd3dProtonD3D12)
        return "DX12 (VKD3D-Proton)";
    return wineVkd3d ? "DX12 (VKD3D)" : "DX12";
}

// A label of an API that DXUP translates, when CE's own hook for that API draws on DXUP's device.
inline std::string WithDxup(const char* label) {
    return std::string(label ? label : "") + " (DXUP)";
}

inline unsigned MergeD3D11Minor(unsigned currentMinor, unsigned observedMinor) {
    return (std::min)(4u, (std::max)(currentMinor, observedMinor));
}

inline const char* D3D11Label(unsigned minorVersion, bool isDxvk) {
    static constexpr const char* kNativeLabels[] = {"DX11", "DX11.1", "DX11.2", "DX11.3", "DX11.4"};
    static constexpr const char* kDxvkLabels[] = {"DX11 (DXVK)", "DX11.1 (DXVK)", "DX11.2 (DXVK)", "DX11.3 (DXVK)",
                                                  "DX11.4 (DXVK)"};
    const unsigned clampedVersion = (std::min)(minorVersion, 4u);
    return isDxvk ? kDxvkLabels[clampedVersion] : kNativeLabels[clampedVersion];
}

// DXUP (Direct3D on Direct3D 12; its DLLs carry ProductName "DXUP") translates DirectDraw with
// D3D6/7, D3D8, D3D9 and D3D11 to one native D3D12 swapchain, which owns CE's overlay. Its legacy
// frontends render through DXUP's own d3d11.dll and dxgi.dll, so those are loaded for every DXUP
// game, while a frontend's own DLL only for its API (d3d8 and ddraw link the D3D9 core statically,
// they do not load d3d9.dll). DXUP has no D3D10 frontend (its src/d3d10 is a placeholder, 0.8.1).
enum class DxupFrontend : unsigned char { kNone, kDirectDraw, kD3D8, kD3D9, kD3D11 };

// `*Loaded`: that DXUP DLL is in the process; `*Evidence`: CE saw the application's device or
// presentation surface for that API. Evidence wins over mere presence (a D3D9 game that also maps
// DirectDraw for its movies is D3D9); without any, the oldest loaded frontend is the one the game
// called, since older frontends load newer DXUP DLLs and never the reverse.
inline DxupFrontend SelectDxupFrontend(bool ddrawLoaded, bool d3d8Loaded, bool d3d9Loaded, bool d3d11Loaded,
                                       bool ddrawEvidence, bool d3d9Evidence) {
    if (d3d9Loaded && d3d9Evidence)
        return DxupFrontend::kD3D9;
    if (ddrawLoaded && ddrawEvidence)
        return DxupFrontend::kDirectDraw;
    if (ddrawLoaded)
        return DxupFrontend::kDirectDraw;
    if (d3d8Loaded)
        return DxupFrontend::kD3D8;
    if (d3d9Loaded)
        return DxupFrontend::kD3D9;
    return d3d11Loaded ? DxupFrontend::kD3D11 : DxupFrontend::kNone;
}

// The overlay label of a DXUP game: the application's API with the translator, at the detail CE's
// own hooks recorded for that API (D3D6/7 over DirectDraw, D3D9Ex, the D3D11 revision).
inline std::string DxupFrontendLabel(DxupFrontend frontend, DirectDrawVersion directDrawVersion,
                                     unsigned legacyD3DVersion, bool d3d9Ex, unsigned d3d11Minor) {
    std::string label;
    switch (frontend) {
        case DxupFrontend::kDirectDraw:
            label = LegacyDirectXLabel(directDrawVersion, legacyD3DVersion);
            break;
        case DxupFrontend::kD3D8:
            label = "DX8";
            break;
        case DxupFrontend::kD3D9:
            label = D3D9Label(d3d9Ex, false);
            break;
        case DxupFrontend::kD3D11:
            label = D3D11Label(d3d11Minor, false);
            break;
        default:
            return {};
    }
    return label + " (DXUP)";
}

inline bool ParseOpenGLVersion(const char* versionString, int* major, int* minor) {
    if (!versionString || !major || !minor)
        return false;

    const char* cursor = versionString;
    while (*cursor && !std::isdigit(static_cast<unsigned char>(*cursor)))
        ++cursor;
    if (!*cursor)
        return false;

    char* majorEnd = nullptr;
    const long parsedMajor = std::strtol(cursor, &majorEnd, 10);
    if (majorEnd == cursor || *majorEnd != '.')
        return false;

    char* minorEnd = nullptr;
    const long parsedMinor = std::strtol(majorEnd + 1, &minorEnd, 10);
    if (minorEnd == majorEnd + 1 || parsedMajor < 1 || parsedMajor > 99 || parsedMinor < 0 || parsedMinor > 99)
        return false;

    *major = static_cast<int>(parsedMajor);
    *minor = static_cast<int>(parsedMinor);
    return true;
}

inline OpenGLIdentity ResolveOpenGLIdentity(const char* versionString, unsigned profileMask) {
    OpenGLIdentity identity;
    identity.valid = ParseOpenGLVersion(versionString, &identity.major, &identity.minor);
    if (!identity.valid)
        return identity;

    const bool profileDefined = identity.major > 3 || (identity.major == 3 && identity.minor >= 2);
    if (profileDefined) {
        if (profileMask & 0x00000001u)
            identity.profile = OpenGLProfile::Core;
        else if (profileMask & 0x00000002u)
            identity.profile = OpenGLProfile::Compatibility;
    }
    return identity;
}

inline std::string FormatOpenGLLabel(const OpenGLIdentity& identity) {
    if (!identity.valid)
        return "OpenGL";

    char label[32] = {};
    const char* profileSuffix = "";
    if (identity.profile == OpenGLProfile::Core)
        profileSuffix = " Core";
    else if (identity.profile == OpenGLProfile::Compatibility)
        profileSuffix = " Compat";
    std::snprintf(label, sizeof(label), "OpenGL %d.%d%s", identity.major, identity.minor, profileSuffix);
    return label;
}

inline bool LabelsDiffer(const char* currentLabel, const char* nextLabel) {
    if (!currentLabel)
        currentLabel = "";
    if (!nextLabel)
        nextLabel = "";
    return std::strcmp(currentLabel, nextLabel) != 0;
}

}  // namespace ce::graphics_api_identity
