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

inline unsigned MergeD3D11Minor(unsigned currentMinor, unsigned observedMinor) {
    return (std::min)(4u, (std::max)(currentMinor, observedMinor));
}

inline const char* D3D11Label(unsigned minorVersion, bool isDxvk) {
    static constexpr const char* kNativeLabels[] = {"DX11", "DX11.1", "DX11.2", "DX11.3", "DX11.4"};
    static constexpr const char* kDxvkLabels[] = {"DX11 (DXVK)", "DX11.1 (DXVK)", "DX11.2 (DXVK)",
                                                  "DX11.3 (DXVK)", "DX11.4 (DXVK)"};
    const unsigned clampedVersion = (std::min)(minorVersion, 4u);
    return isDxvk ? kDxvkLabels[clampedVersion] : kNativeLabels[clampedVersion];
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
